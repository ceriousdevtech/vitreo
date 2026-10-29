/// @file win32_full_path.cpp
/// @brief GetFullPathNameW normalization. See win32_full_path.hpp.

#include "capsule/vfs/win32_full_path.hpp"

#include <cctype>
#include <vector>

namespace vitreo::capsule {
namespace {

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(a[i])) !=
            std::toupper(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

/// Legacy DOS device names. A bare device name is a device path
/// (files_ns_probe gfp_con: "CON" -> "\\.\CON"); Windows 11 no longer treats
/// "C:\a\nul.txt" as the device (gfp_nul_ext), so only the whole input counts.
bool is_dos_device(std::string_view s) {
    for (const char* d : {"CON", "PRN", "AUX", "NUL"}) {
        if (ieq(s, d)) return true;
    }
    if (s.size() == 4 && s[3] >= '1' && s[3] <= '9' &&
        (ieq(s.substr(0, 3), "COM") || ieq(s.substr(0, 3), "LPT"))) {
        return true;
    }
    return false;
}

bool is_drive(std::string_view p) {
    return p.size() >= 2 && p[1] == ':' &&
           std::isalpha(static_cast<unsigned char>(p[0]));
}

/// Split into root and remainder. Root forms: "X:\", "\\server\share\",
/// "\\?\", "\\.\". Returns false for a non-absolute path.
bool split_root(const std::string& p, std::string& root, std::string& rest) {
    if (p.size() >= 4 && p[0] == '\\' && p[1] == '\\' &&
        (p[2] == '?' || p[2] == '.') && p[3] == '\\') {
        root = p.substr(0, 4);
        rest = p.substr(4);
        return true;
    }
    if (p.size() >= 2 && p[0] == '\\' && p[1] == '\\') {
        // UNC: "\\server\share\..." — ".." never climbs above the share
        // (gfp_unc_dotdot).
        usize i = 2;
        while (i < p.size() && p[i] == '\\') ++i;
        const usize server_end = p.find('\\', i);
        if (server_end == std::string::npos) {
            root = p + "\\";
            rest.clear();
            return true;
        }
        usize share_begin = server_end + 1;
        const usize share_end = p.find('\\', share_begin);
        if (share_end == std::string::npos) {
            root = p + "\\";
            rest.clear();
            return true;
        }
        root = p.substr(0, share_end + 1);
        rest = p.substr(share_end + 1);
        return true;
    }
    if (is_drive(p) && p.size() >= 3 && p[2] == '\\') {
        root = p.substr(0, 3);
        rest = p.substr(3);
        return true;
    }
    return false;
}

std::string cwd_root(std::string_view cwd) {
    std::string c(cwd);
    for (char& ch : c) if (ch == '/') ch = '\\';
    std::string root, rest;
    if (split_root(c, root, rest)) return root;
    if (is_drive(c)) return c.substr(0, 2) + "\\";
    return "C:\\";
}

}  // namespace

std::optional<Win32FullPath> win32_full_path(std::string_view in,
                                             std::string_view cwd_in) {
    if (in.empty()) return std::nullopt;
    std::string p(in);
    for (char& ch : p) if (ch == '/') ch = '\\';
    std::string cwd(cwd_in);
    for (char& ch : cwd) if (ch == '/') ch = '\\';
    while (cwd.size() > 3 && cwd.back() == '\\') cwd.pop_back();

    if (is_dos_device(p)) {
        return Win32FullPath{"\\\\.\\" + p, std::string::npos};
    }

    std::string root, rest;
    if (!split_root(p, root, rest)) {
        std::string combined;
        if (is_drive(p)) {
            // "X:foo": relative to the current directory of drive X. Only
            // the process's own drive has one here; any other drive starts
            // at its root (gfp_drive_relative_other "D:foo" -> "D:\foo",
            // gfp_drive_only_other "U:" -> "U:\").
            const std::string tail = p.substr(2);
            const bool same = is_drive(cwd) &&
                std::toupper(static_cast<unsigned char>(cwd[0])) ==
                    std::toupper(static_cast<unsigned char>(p[0]));
            std::string base = same ? cwd : (p.substr(0, 2) + "\\");
            if (tail.empty()) {
                combined = base;
            } else {
                combined = base + (base.back() == '\\' ? "" : "\\") + tail;
            }
        } else if (p[0] == '\\') {
            combined = cwd_root(cwd) + p.substr(1);
        } else {
            combined = cwd + (cwd.back() == '\\' ? "" : "\\") + p;
        }
        if (!split_root(combined, root, rest)) return std::nullopt;
    }

    // Collapse separators, then apply "." / "..". The final segment loses
    // trailing periods and spaces (gfp_trailing_dots_spaces "foo. ." ->
    // "foo"); inner segments keep them (gfp_trailing_dots_dir "a..").
    const bool trailing_sep = !rest.empty() && rest.back() == '\\';
    std::vector<std::string> raw;
    usize pos = 0;
    while (pos <= rest.size()) {
        const usize next = rest.find('\\', pos);
        const usize end = next == std::string::npos ? rest.size() : next;
        if (end > pos) raw.emplace_back(rest.substr(pos, end - pos));
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    if (!trailing_sep && !raw.empty()) {
        std::string& last = raw.back();
        if (last != "." && last != "..") {
            while (!last.empty() && (last.back() == '.' || last.back() == ' ')) {
                last.pop_back();
            }
            if (last.empty()) raw.pop_back();
        }
    }
    std::vector<std::string> segs;
    for (auto& s : raw) {
        if (s == ".") continue;
        if (s == "..") {
            if (!segs.empty()) segs.pop_back();
            continue;
        }
        segs.push_back(std::move(s));
    }

    Win32FullPath out;
    out.path = root;
    for (usize i = 0; i < segs.size(); ++i) {
        if (i + 1 == segs.size()) out.file_part = out.path.size();
        out.path += segs[i];
        if (i + 1 < segs.size()) out.path.push_back('\\');
    }
    if (segs.empty()) {
        out.file_part = std::string::npos;
    } else if (trailing_sep) {
        out.path.push_back('\\');
        out.file_part = std::string::npos;
    }
    return out;
}

}  // namespace vitreo::capsule

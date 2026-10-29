#pragma once

#include <vitreo/common.hpp>
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace vitreo::winapi {

/// @brief Path manipulation utilities for SHLWAPI functions.
///
/// These are extracted from the thunk implementation to allow direct testing.
class ShlwapiPathUtils {
public:
    /// @brief Convert ch to lowercase if ASCII uppercase.
    static u16 tolower_ascii_u16(u16 ch) {
        if (ch >= 'A' && ch <= 'Z') {
            return static_cast<u16>(ch - 'A' + 'a');
        }
        return ch;
    }

    /// @brief Check if character is a path separator.
    static bool is_path_sep(char c) {
        return c == '\\' || c == '/';
    }

    /// @brief Normalize all forward slashes to backslashes.
    static std::string normalize_seps(std::string s) {
        std::replace(s.begin(), s.end(), '/', '\\');
        return s;
    }

    /// @brief Remove all trailing path separators.
    static std::string strip_trailing_seps(std::string s) {
        while (!s.empty() && is_path_sep(s.back())) {
            s.pop_back();
        }
        return s;
    }

    /// @brief Check if a path is relative (not absolute).
    static bool is_relative_ascii_path(std::string_view p) {
        if (p.empty()) {
            return true;
        }
        // Drive absolute: C:\...
        if (p.size() >= 3 && std::isalpha(static_cast<unsigned char>(p[0])) && 
            p[1] == ':' && is_path_sep(p[2])) {
            return false;
        }
        // UNC: \\server\share\...
        if (p.size() >= 2 && is_path_sep(p[0]) && is_path_sep(p[1])) {
            return false;
        }
        // Rooted: \...
        if (is_path_sep(p[0])) {
            return false;
        }
        return true;
    }

    /// @brief Combine directory and file paths (PathCombineW logic).
    static std::string path_combine(std::string a, std::string b) {
        a = normalize_seps(std::move(a));
        b = normalize_seps(std::move(b));

        if (b.empty()) {
            return a;
        }
        if (!is_relative_ascii_path(b)) {
            return b;
        }
        if (a.empty()) {
            return b;
        }

        a = strip_trailing_seps(std::move(a));
        if (!a.empty()) {
            a.push_back('\\');
        }
        a += b;
        return a;
    }

    /// @brief Find the filename portion of a path.
    /// @return Index of filename start, or 0 if no separator found.
    static size_t find_filename_start(std::string_view path) {
        if (path.empty()) {
            return 0;
        }
        // Find last separator
        auto pos = path.find_last_of("\\/");
        if (pos == std::string_view::npos) {
            return 0;
        }
        return pos + 1;
    }

    /// @brief Find the filename portion of a path (PathFindFileNameW).
    static std::string_view path_find_filename(std::string_view path) {
        return path.substr(find_filename_start(path));
    }

    /// @brief Remove the filename portion from a path (PathRemoveFileSpecW).
    /// @return Modified path and whether a change was made.
    static std::pair<std::string, bool> path_remove_filespec(std::string path) {
        path = normalize_seps(std::move(path));
        path = strip_trailing_seps(std::move(path));
        
        auto pos = path.find_last_of('\\');
        if (pos == std::string::npos) {
            return {path, false};
        }
        path.resize(pos);
        return {path, true};
    }

    /// @brief Add a backslash to the end of a path if not present.
    /// @return The modified path.
    static std::string path_add_backslash(std::string path) {
        path = normalize_seps(std::move(path));
        if (path.empty() || !is_path_sep(path.back())) {
            path.push_back('\\');
        }
        return path;
    }

    /// @brief Find a character in a wide string (StrChrW logic).
    /// @return Index of character, or -1 if not found.
    static ptrdiff_t str_chr_w(std::wstring_view str, wchar_t needle) {
        for (size_t i = 0; i < str.size(); ++i) {
            if (str[i] == needle) {
                return static_cast<ptrdiff_t>(i);
            }
        }
        return -1;
    }

    /// @brief Find a substring case-insensitively (StrStrIW logic).
    /// @return Index of substring, or -1 if not found.
    static ptrdiff_t str_str_iw(std::wstring_view hay, std::wstring_view needle) {
        if (needle.empty()) {
            return 0;
        }
        if (hay.size() < needle.size()) {
            return -1;
        }

        for (size_t start = 0; start <= hay.size() - needle.size(); ++start) {
            bool match = true;
            for (size_t j = 0; j < needle.size(); ++j) {
                wchar_t h = static_cast<wchar_t>(tolower_ascii_u16(static_cast<u16>(hay[start + j])));
                wchar_t n = static_cast<wchar_t>(tolower_ascii_u16(static_cast<u16>(needle[j])));
                if (h != n) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return static_cast<ptrdiff_t>(start);
            }
        }
        return -1;
    }
};

} // namespace vitreo::winapi

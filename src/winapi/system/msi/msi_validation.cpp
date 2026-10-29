// SPDX-License-Identifier: LGPL-2.1-only
/// @file msi_validation.cpp
/// @brief `_Validation` checking for MsiViewModify's validate modes.
///
/// Contract: Learn "_Validation Table", "Column Data Types" and the
/// MSIDBERROR values of MsiQuery.h. Learn leaves each category's exact
/// acceptance open; the rules below are the ones measured by
/// tools/oracle_probe/msi_database_modify_probe.c and
/// msi_database_validate2_probe.c (x86 == x64). Notable measured facts:
/// category names are case-sensitive ("Guid", not "GUID"); the Set column
/// is checked only for integers and for strings with no Category; a
/// DefaultDir value is accepted only in identifier form; a column with no
/// `_Validation` row reports MSIDBERROR_MISSINGDATA.

#include "winapi/system/msi/msi_validation.hpp"
#include "winapi/system/msi/msi_engine.hpp"

#include <algorithm>
#include <cctype>

namespace vitreo::winapi::msi {

namespace {

// MSIDBERROR (MsiQuery.h).
enum : i32 {
    kNoError = 0, kDuplicateKey = 1, kRequired = 2, kBadLink = 3, kOverflow = 4, kUnderflow = 5,
    kNotInSet = 6, kBadVersion = 7, kBadCase = 8, kBadGuid = 9, kBadWildcard = 10,
    kBadIdentifier = 11, kBadLanguage = 12, kBadFilename = 13, kBadPath = 14, kBadCondition = 15,
    kBadFormatted = 16, kBadTemplate = 17, kBadDefaultDir = 18, kBadRegPath = 19,
    kBadCustomSource = 20, kBadProperty = 21, kMissingData = 22, kBadCategory = 23,
    kBadKeyTable = 24, kBadMaxMinValues = 25, kBadCabinet = 26, kBadShortcut = 27,
    kStringOverflow = 28,
};

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    usize start = 0;
    for (;;) {
        const usize p = s.find(sep, start);
        out.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (p == std::string::npos) return out;
        start = p + 1;
    }
}

bool is_identifier(const std::string& s) {
    if (s.empty()) return false;
    const unsigned char c0 = static_cast<unsigned char>(s[0]);
    if (!std::isalpha(c0) && c0 != '_') return false;
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && c != '_' && c != '.') return false;
    }
    return true;
}

bool long_name_chars(const std::string& s, bool wildcards) {
    if (s.empty()) return false;
    for (const char c : s) {
        if (static_cast<unsigned char>(c) < 0x20) return false;
        switch (c) {
        case '\\': case '/': case ':': case '"': case '<': case '>': case '|': return false;
        case '*': case '?': if (!wildcards) return false; break;
        default: break;
        }
    }
    return true;
}

/// An 8.3 name: at most one dot, name 1-8, extension 0-3, no space or
/// + , ; = [ ] (measured "a b.txt", "a+b.txt", "abc.text" fail).
bool short_name(const std::string& s, bool wildcards) {
    if (!long_name_chars(s, wildcards)) return false;
    for (const char c : s) {
        if (c == ' ' || c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']') return false;
    }
    const usize dot = s.find('.');
    if (dot != std::string::npos && s.find('.', dot + 1) != std::string::npos) return false;
    const usize name_len = dot == std::string::npos ? s.size() : dot;
    const usize ext_len = dot == std::string::npos ? 0 : s.size() - dot - 1;
    return name_len >= 1 && name_len <= 8 && ext_len <= 3;
}

bool filename(const std::string& s, bool wildcards) {
    const auto parts = split(s, '|');
    if (parts.size() > 2) return false;
    if (!short_name(parts[0], wildcards)) return false;
    return parts.size() == 1 || long_name_chars(parts[1], wildcards);
}

bool absolute_path(const std::string& s) {
    if (!s.empty() && s[0] == '[') return true;  // a formatted directory reference
    if (s.size() >= 2 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':')
        return s.size() == 2 || s[2] == '\\';
    return s.size() >= 2 && s[0] == '\\' && s[1] == '\\';
}

/// Bracket structure of a Formatted / Template string: `[\x]` escapes,
/// nested references, no empty `[]`, no stray `]`.
bool brackets(const std::string& s, bool allow_empty) {
    int depth = 0;
    std::vector<usize> content;
    for (usize i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '[') {
            // [\x]: one escaped character.
            if (i + 3 < s.size() && s[i + 1] == '\\' && s[i + 3] == ']') { i += 3; continue; }
            ++depth;
            content.push_back(i);
        } else if (c == ']') {
            if (depth == 0) return false;
            if (!allow_empty && content.back() + 1 == i) return false;
            content.pop_back();
            --depth;
        }
    }
    return depth == 0;
}

bool version(const std::string& s) {
    const auto parts = split(s, '.');
    if (parts.size() > 4) return false;
    for (const std::string& p : parts) {
        if (p.empty() || p.size() > 5) return false;
        for (const char c : p) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        if (std::stoul(p) > 65535) return false;
    }
    return true;
}

bool language(const std::string& s) {
    for (std::string p : split(s, ',')) {
        p.erase(0, p.find_first_not_of(' '));
        p.erase(p.find_last_not_of(' ') + 1);
        if (p.empty() || p.size() > 5) return false;
        for (const char c : p) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        if (std::stoul(p) > 65535) return false;
    }
    return true;
}

bool guid(const std::string& s) {
    if (s.size() != 38 || s.front() != '{' || s.back() != '}') return false;
    for (usize i = 1; i < 37; ++i) {
        const char c = s[i];
        if (i == 9 || i == 14 || i == 19 || i == 24) { if (c != '-') return false; continue; }
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

}  // namespace

i32 validate_category(const std::string& category, const std::string& v, bool root_dir) {
    (void)root_dir;
    if (category == "Text") return kNoError;
    if (category == "UpperCase")
        return std::any_of(v.begin(), v.end(), [](char c) { return std::islower(static_cast<unsigned char>(c)); }) ? kBadCase : kNoError;
    if (category == "LowerCase")
        return std::any_of(v.begin(), v.end(), [](char c) { return std::isupper(static_cast<unsigned char>(c)); }) ? kBadCase : kNoError;
    if (category == "Identifier") return is_identifier(v) ? kNoError : kBadIdentifier;
    if (category == "Property") return is_identifier(!v.empty() && v[0] == '%' ? v.substr(1) : v) ? kNoError : kBadProperty;
    if (category == "Filename") return filename(v, false) ? kNoError : kBadFilename;
    if (category == "WildCardFilename") return filename(v, true) ? kNoError : kBadWildcard;
    if (category == "Path") return absolute_path(v) ? kNoError : kBadPath;
    if (category == "Paths") {
        for (const std::string& p : split(v, ';')) if (!absolute_path(p)) return kBadPath;
        return kNoError;
    }
    if (category == "AnyPath") {
        if (std::count(v.begin(), v.end(), '|') > 1) return kBadPath;
        for (const char c : v) if (c == '*' || c == '?' || c == '"' || c == '<' || c == '>') return kBadPath;
        return kNoError;
    }
    if (category == "DefaultDir") return is_identifier(v) ? kNoError : kBadDefaultDir;
    if (category == "RegPath") {
        if (!v.empty() && (v.front() == '\\' || v.back() == '\\')) return kBadRegPath;
        return v.find("\\\\") == std::string::npos ? kNoError : kBadRegPath;
    }
    if (category == "Formatted" || category == "FormattedSDDLText") return brackets(v, false) ? kNoError : kBadFormatted;
    if (category == "Template") return brackets(v, false) ? kNoError : kBadTemplate;
    if (category == "Condition") {
        // The engine's own conditional grammar decides what parses.
        const std::string trimmed = v.substr(0, v.find_last_not_of(' ') + 1);
        if (trimmed == "()") return kNoError;
        Session s;
        return s.evaluate_condition(v).has_value() ? kNoError : kBadCondition;
    }
    if (category == "Guid") return guid(v) ? kNoError : kBadGuid;
    if (category == "Version") return version(v) ? kNoError : kBadVersion;
    if (category == "Language") return language(v) ? kNoError : kBadLanguage;
    if (category == "CustomSource") return kNoError;
    if (category == "Cabinet") return long_name_chars(!v.empty() && v[0] == '#' ? v.substr(1) : v, false) ? kNoError : kBadCabinet;
    if (category == "Shortcut") return (is_identifier(v) || (v.find('[') != std::string::npos && brackets(v, false))) ? kNoError : kBadShortcut;
    return kBadCategory;
}

u32 validate_row(Package& pkg, const Table& table, const std::vector<std::string>& cells,
                 const std::vector<bool>& present, ValidateMode mode, std::optional<usize> row,
                 std::vector<std::pair<std::string, i32>>& errors) {
    constexpr u32 kFunctionFailed = 1627, kInvalidData = 13;
    errors.clear();
    const Table* val = pkg.find_table("_Validation");
    if (val == nullptr) return kFunctionFailed;
    auto col = [&](const char* n) { return val->exact_column(n); };
    const auto c_table = col("Table"), c_column = col("Column"), c_nullable = col("Nullable"),
               c_min = col("MinValue"), c_max = col("MaxValue"), c_keytable = col("KeyTable"),
               c_keycol = col("KeyColumn"), c_category = col("Category"), c_set = col("Set");
    if (!c_table || !c_column) return kFunctionFailed;
    auto cell = [&](const std::vector<std::string>& r, const std::optional<usize>& c) -> std::string {
        return c && *c < r.size() ? r[*c] : std::string{};
    };

    if (mode == ValidateMode::Delete) {
        // Rows of other tables whose foreign key names this row block it; the
        // error is reported against the referenced key column (measured
        // MSIDBERROR_REQUIRED).
        if (!row) return kFunctionFailed;
        for (const auto& vr : val->rows) {
            const auto tables = split(cell(vr, c_keytable), ';');
            if (std::find(tables.begin(), tables.end(), table.name) == tables.end()) continue;
            const auto kc = cell_int(cell(vr, c_keycol));
            if (!kc || *kc < 1 || static_cast<usize>(*kc) > table.columns.size()) continue;
            const std::string& key = table.rows[*row][static_cast<usize>(*kc - 1)];
            const Table* src = pkg.find_table(cell(vr, c_table));
            const auto sc = src ? src->exact_column(cell(vr, c_column)) : std::nullopt;
            if (!sc) continue;
            for (const auto& sr : src->rows) {
                if (sr[*sc] == key) {
                    errors.emplace_back(table.columns[static_cast<usize>(*kc - 1)].name, kRequired);
                    break;
                }
            }
        }
        return errors.empty() ? 0u : kInvalidData;
    }

    const usize keys = table.key_count();
    for (usize c = 0; c < table.columns.size(); ++c) {
        if (c < present.size() && !present[c]) continue;
        const ColumnDef& def = table.columns[c];
        const std::vector<std::string>* vrow = nullptr;
        for (const auto& vr : val->rows) {
            if (cell(vr, c_table) == table.name && cell(vr, c_column) == def.name) { vrow = &vr; break; }
        }
        if (vrow == nullptr) { errors.emplace_back(def.name, kMissingData); continue; }
        const std::string& value = c < cells.size() ? cells[c] : std::string{};

        if (mode == ValidateMode::New && c == 0 && keys > 0) {
            if (pkg.find_row(table, std::span<const std::string>(cells.data(), std::min(keys, cells.size())))) {
                errors.emplace_back(def.name, kDuplicateKey);
                continue;
            }
        }
        if (value.empty()) {
            if (cell(*vrow, c_nullable) != "Y") errors.emplace_back(def.name, kRequired);
            continue;
        }
        const std::string category = cell(*vrow, c_category);
        const std::string set = cell(*vrow, c_set);
        i32 error = kNoError;
        if (def.is_integer()) {
            const auto v = cell_int(value).value_or(0);
            const auto mn = cell_int(cell(*vrow, c_min));
            const auto mx = cell_int(cell(*vrow, c_max));
            if (mn && mx && *mx < *mn) error = kBadMaxMinValues;
            else if (mn && v < *mn) error = kUnderflow;
            else if (mx && v > *mx) error = kOverflow;
            else if (!set.empty()) {
                const auto items = split(set, ';');
                if (std::find(items.begin(), items.end(), std::to_string(v)) == items.end()) error = kNotInSet;
            }
        } else if (def.is_string) {
            usize units = 0;
            for (const char ch : value) if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) ++units;
            if (def.size() != 0 && units > def.size()) error = kStringOverflow;
            else if (!category.empty()) error = validate_category(category, value, false);
            else if (!set.empty()) {
                const auto items = split(set, ';');
                if (std::find(items.begin(), items.end(), value) == items.end()) error = kNotInSet;
            }
        } else if (def.is_stream() && !category.empty() && category != "Binary") {
            error = kBadCategory;
        }
        if (error == kNoError && !cell(*vrow, c_keytable).empty()) {
            const auto kc = cell_int(cell(*vrow, c_keycol)).value_or(1);
            bool found = false, any_table = false;
            for (const std::string& kt : split(cell(*vrow, c_keytable), ';')) {
                const Table* ref = pkg.find_table(kt);
                if (!ref) continue;
                any_table = true;
                if (kc < 1 || static_cast<usize>(kc) > ref->columns.size()) continue;
                for (const auto& rr : ref->rows) {
                    if (rr[static_cast<usize>(kc - 1)] == value) { found = true; break; }
                }
                if (found) break;
            }
            if (!any_table) error = kBadKeyTable;
            else if (!found) error = kBadLink;
        }
        if (error != kNoError) errors.emplace_back(def.name, error);
    }
    return errors.empty() ? 0u : kInvalidData;
}

}  // namespace vitreo::winapi::msi
